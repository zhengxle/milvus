package proxy

import (
	"encoding/json"
	"fmt"
	"reflect"

	"github.com/cockroachdb/errors"
	"go.uber.org/zap"

	"github.com/milvus-io/milvus-proto/go-api/v2/commonpb"
	"github.com/milvus-io/milvus-proto/go-api/v2/milvuspb"
	"github.com/milvus-io/milvus/pkg/log"
	"github.com/milvus-io/milvus/pkg/util/funcutil"
	"github.com/milvus-io/milvus/pkg/util/merr"
)

type rankType int

const (
	invalidRankType  rankType = iota // invalidRankType   = 0
	rrfRankType                      // rrfRankType = 1
	weightedRankType                 // weightedRankType = 2
	udfExprRankType                  // udfExprRankType = 3
)

var rankTypeMap = map[string]rankType{
	"invalid":  invalidRankType,
	"rrf":      rrfRankType,
	"weighted": weightedRankType,
	"expr":     udfExprRankType,
}

type reScorer interface {
	name() string
	scorerType() rankType
	reScore(input *milvuspb.SearchResults)
}

type baseScorer struct {
	scorerName string
}

func (bs *baseScorer) name() string {
	return bs.scorerName
}

type rrfScorer struct {
	baseScorer
	k float32
}

func (rs *rrfScorer) reScore(input *milvuspb.SearchResults) {
	for i := range input.Results.GetScores() {
		input.Results.Scores[i] = 1 / (rs.k + float32(i+1))
	}
}

func (rs *rrfScorer) scorerType() rankType {
	return rrfRankType
}

type weightedScorer struct {
	baseScorer
	weight float32
}

func (ws *weightedScorer) reScore(input *milvuspb.SearchResults) {
	for i, score := range input.Results.GetScores() {
		input.Results.Scores[i] = ws.weight * score
	}
}

func (ws *weightedScorer) scorerType() rankType {
	return weightedRankType
}

func NewReScorer(reqs []*milvuspb.SearchRequest, rankParams []*commonpb.KeyValuePair) ([]reScorer, error) {
	res := make([]reScorer, len(reqs))
	rankTypeStr, err := funcutil.GetAttrByKeyFromRepeatedKV(RankTypeKey, rankParams)
	if err != nil {
		log.Info("rank strategy not specified, use rrf instead")
		// if not set rank strategy, use rrf rank as default
		for i := range reqs {
			res[i] = &rrfScorer{
				baseScorer: baseScorer{
					scorerName: "rrf",
				},
				k: float32(defaultRRFParamsValue),
			}
		}
		return res, nil
	}

	if _, ok := rankTypeMap[rankTypeStr]; !ok {
		return nil, errors.Errorf("unsupported rank type %s", rankTypeStr)
	}

	paramStr, err := funcutil.GetAttrByKeyFromRepeatedKV(RankParamsKey, rankParams)
	if err != nil {
		return nil, errors.New(RankParamsKey + " not found in rank_params")
	}

	var params map[string]interface{}
	err = json.Unmarshal([]byte(paramStr), &params)
	if err != nil {
		return nil, err
	}

	switch rankTypeMap[rankTypeStr] {
	case rrfRankType:
		_, ok := params[RRFParamsKey]
		if !ok {
			return nil, errors.New(RRFParamsKey + " not found in rank_params")
		}
		var k float64
		if reflect.ValueOf(params[RRFParamsKey]).CanFloat() {
			k = reflect.ValueOf(params[RRFParamsKey]).Float()
		} else {
			return nil, errors.New("The type of rank param k should be float")
		}
		if k <= 0 || k >= maxRRFParamsValue {
			return nil, errors.New("The rank params k should be in range (0, 16384)")
		}
		log.Debug("rrf params", zap.Float64("k", k))
		for i := range reqs {
			res[i] = &rrfScorer{
				baseScorer: baseScorer{
					scorerName: "rrf",
				},
				k: float32(k),
			}
		}
	case weightedRankType:
		if _, ok := params[WeightsParamsKey]; !ok {
			return nil, errors.New(WeightsParamsKey + " not found in rank_params")
		}
		weights := make([]float32, 0)
		switch reflect.TypeOf(params[WeightsParamsKey]).Kind() {
		case reflect.Slice:
			rs := reflect.ValueOf(params[WeightsParamsKey])
			for i := 0; i < rs.Len(); i++ {
				v := rs.Index(i).Elem()
				if v.CanFloat() {
					weight := v.Float()
					if weight < 0 || weight > 1 {
						return nil, errors.New("rank param weight should be in range [0, 1]")
					}
					weights = append(weights, float32(weight))
				} else {
					return nil, errors.New("The type of rank param weight should be float")
				}
			}
		default:
			return nil, errors.New("The weights param should be an array")
		}

		log.Debug("weights params", zap.Any("weights", weights))
		if len(reqs) != len(weights) {
			return nil, merr.WrapErrParameterInvalid(fmt.Sprint(len(reqs)), fmt.Sprint(len(weights)), "the length of weights param mismatch with ann search requests")
		}
		for i := range reqs {
			res[i] = &weightedScorer{
				baseScorer: baseScorer{
					scorerName: "weighted",
				},
				weight: weights[i],
			}
		}
	default:
		return nil, errors.Errorf("unsupported rank type %s", rankTypeStr)
	}

	return res, nil
}
